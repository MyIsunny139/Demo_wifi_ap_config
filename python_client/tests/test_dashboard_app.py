import logging
from pathlib import Path
from unittest.mock import AsyncMock, Mock

import pytest
from fastapi.testclient import TestClient

from python_client import dashboard_app
from python_client.dashboard_app import create_app


HOST = "192.168.1.3"


def make_app(tmp_path: Path):
    return create_app(host=HOST, capture_dir=tmp_path / "captures")


def test_index_falls_back_then_reads_dashboard_file(monkeypatch, tmp_path):
    monkeypatch.setattr(dashboard_app, "STATIC_DIR", tmp_path)
    app = make_app(tmp_path)

    with TestClient(app) as client:
        fallback = client.get("/")
        (tmp_path / "dashboard.html").write_text(
            "<html><body>dashboard from Task 5</body></html>", encoding="utf-8"
        )
        dashboard = client.get("/")

    assert fallback.status_code == 200
    assert "Lab-Environment-Monitoring" in fallback.text
    assert dashboard.text == "<html><body>dashboard from Task 5</body></html>"


def test_missing_static_directory_returns_not_found(monkeypatch, tmp_path):
    missing_static_dir = tmp_path / "missing-static"
    monkeypatch.setattr(dashboard_app, "STATIC_DIR", missing_static_dir)
    app = make_app(tmp_path)

    with TestClient(app) as client:
        response = client.get("/static/dashboard.css")

    assert response.status_code == 404


def test_existing_static_directory_is_mounted(monkeypatch, tmp_path):
    static_dir = tmp_path / "static"
    static_dir.mkdir()
    (static_dir / "dashboard.css").write_text(
        "body { color: black; }", encoding="utf-8"
    )
    monkeypatch.setattr(dashboard_app, "STATIC_DIR", static_dir)
    app = make_app(tmp_path)

    with TestClient(app) as client:
        response = client.get("/static/dashboard.css")

    assert response.status_code == 200
    assert response.text == "body { color: black; }"


def test_dashboard_static_assets_and_required_controls_are_served(tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        page = client.get("/")
        stylesheet = client.get("/static/dashboard.css")
        script = client.get("/static/dashboard.js")

    assert page.status_code == 200
    assert stylesheet.status_code == 200
    assert script.status_code == 200
    for text in (
        "实时视频",
        "实时预警",
        "ESP32 地址",
        "应用地址",
        "启动处理",
        "停止处理",
        "最大允许人数",
        "应用阈值",
        "截图",
        "保存帧",
        "保存路径",
        "RX FPS",
        "AI FPS",
        "抽烟人数",
        "最高置信度",
    ):
        assert text in page.text or not text.isascii()
    for element_id in (
        "videoFrame",
        "connectionStatus",
        "processingStatus",
        "esp32Address",
        "applyAddressButton",
        "warningMessage",
        "maxPeopleInput",
        "toastRegion",
        "maxPeopleValue",
        "confidenceValue",
        "dhtChart",
        "mq2Chart",
        "temperatureMaxInput",
        "humidityMaxInput",
        "mq2MaxInput",
    ):
        assert f'id="{element_id}"' in page.text


def test_dashboard_styles_cover_accessibility_and_responsive_layout(tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        page = client.get("/")
        stylesheet = client.get("/static/dashboard.css")

    assert 'lang="zh-CN"' in page.text
    assert 'aria-live="polite"' in page.text
    assert "focus-visible" in stylesheet.text
    assert "prefers-reduced-motion" in stylesheet.text
    assert "aspect-ratio" in stylesheet.text
    assert "overflow-x: hidden" in stylesheet.text
    for breakpoint in ("320px", "768px", "1024px", "1440px"):
        assert breakpoint in stylesheet.text


def test_dashboard_script_uses_only_local_api_with_resilient_interactions(tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        page = client.get("/")
        script = client.get("/static/dashboard.js")

    assert 'fetch("/api/state")' in script.text
    assert 'location.protocol === "https:" ? "wss:" : "ws:"' in script.text
    assert 'new WebSocket(`${socketProtocol}//${location.host}/ws/state`)' in script.text
    assert '"/frame/latest.jpg"' in script.text
    assert 'postJson("/api/esp32"' in script.text
    assert 'addEventListener("input"' in script.text
    assert "addressDirty" in script.text
    assert "localStorage.setItem" in script.text
    assert "button.disabled = true" in script.text
    assert "reconnectDelay" in script.text
    assert "showToast" in script.text
    assert "syncTerminalAlarm" not in script.text
    assert "AudioContext" not in script.text
    assert "alarmTimer" not in script.text
    assert 'fetch("/api/diagnostics/browser"' in script.text
    assert "frame_sequence" in script.text
    assert "frameSequence === requestedFrameSequence" in script.text
    assert "retriedFrameSequence !== failedSequence" in script.text
    assert "videoFrame.addEventListener(\"error\"" in script.text
    assert "ws://192.168" not in script.text
    assert "readonly" not in page.text
    assert "dashboard.js?v=20261028-people-threshold1" in page.text


def test_state_endpoint_returns_service_snapshot(tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        response = client.get("/api/state")

    assert response.status_code == 200
    assert response.json()["running"] is False
    assert response.json()["esp32_host"] == HOST
    assert response.json()["esp32_port"] == 81


@pytest.mark.parametrize(
    ("endpoint", "method_name", "running"),
    [
        ("/api/start", "start", True),
        ("/api/stop", "stop", False),
    ],
)
def test_start_and_stop_endpoints_call_service(
    endpoint, method_name, running, tmp_path
):
    app = make_app(tmp_path)
    service = app.state.dashboard_service

    async def update_running():
        service.state.running = running

    method = AsyncMock(side_effect=update_running)
    setattr(service, method_name, method)

    with TestClient(app) as client:
        response = client.post(endpoint)

    assert response.status_code == 200
    assert response.json()["running"] is running
    expected_awaits = 2 if method_name == "stop" else 1
    assert method.await_count == expected_awaits
    method.assert_awaited_with()


def test_esp32_endpoint_updates_connection_after_trimming_host(tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        response = client.post(
            "/api/esp32", json={"host": " 192.168.166.200 ", "port": 81}
        )

    assert response.status_code == 200
    assert response.json()["esp32_host"] == "192.168.166.200"
    assert response.json()["esp32_port"] == 81
    assert app.state.dashboard_service.uri == "ws://192.168.166.200:81/video"


@pytest.mark.parametrize(
    "payload",
    [
        {"host": "", "port": 81},
        {"host": "http://192.168.1.3", "port": 81},
        {"host": "192.168.1.3:81", "port": 81},
        {"host": True, "port": 81},
        {"host": "192.168.1.3", "port": 0},
        {"host": "192.168.1.3", "port": 65536},
    ],
)
def test_esp32_endpoint_rejects_invalid_connection(payload, tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        response = client.post("/api/esp32", json=payload)

    assert response.status_code == 422


def test_esp32_endpoint_rejects_change_while_running(tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    service.state.running = True

    with TestClient(app) as client:
        response = client.post(
            "/api/esp32", json={"host": "192.168.166.200", "port": 81}
        )

    assert response.status_code == 409
    assert response.json()["detail"] == "请先停止处理，再修改 ESP32 地址"
    assert service.host == HOST


def test_threshold_endpoint_validates_and_updates_state(tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        response = client.post(
            "/api/thresholds",
            json={"medium_smoking": 2, "high_smoking": 5},
        )

    assert response.status_code == 200
    assert response.json()["medium_smoking"] == 2
    assert response.json()["high_smoking"] == 5


def test_environment_thresholds_update_state(tmp_path):
    app = make_app(tmp_path)
    with TestClient(app) as client:
        response = client.post(
            "/api/thresholds",
            json={"max_people": 3, "temperature_max": 32.5,
                  "humidity_max": 65.0, "mq2_max": 1500},
        )
    assert response.status_code == 200
    assert response.json()["temperature_max"] == 32.5
    assert response.json()["humidity_max"] == 65.0
    assert response.json()["mq2_max"] == 1500


def test_threshold_endpoint_rejects_invalid_order(tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    service.update_thresholds = AsyncMock()

    with TestClient(app) as client:
        response = client.post(
            "/api/thresholds",
            json={"medium_smoking": 5, "high_smoking": 5},
        )

    assert response.status_code == 400
    service.update_thresholds.assert_not_awaited()


@pytest.mark.parametrize(
    "payload",
    [
        {"medium_smoking": True, "high_smoking": 5},
        {"medium_smoking": 2.0, "high_smoking": 5},
        {"medium_smoking": 2, "high_smoking": False},
        {"medium_smoking": 2, "high_smoking": 5.0},
    ],
)
def test_threshold_endpoint_rejects_bool_and_float_values(payload, tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    service.update_thresholds = AsyncMock()

    with TestClient(app) as client:
        response = client.post("/api/thresholds", json=payload)

    assert response.status_code == 422
    service.update_thresholds.assert_not_awaited()


@pytest.mark.parametrize("endpoint", ["/api/screenshot", "/api/save"])
def test_capture_endpoints_return_conflict_without_a_frame(endpoint, tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        response = client.post(endpoint)

    assert response.status_code == 409
    assert response.json()["detail"] == "no frame available"


@pytest.mark.parametrize(
    ("endpoint", "prefix"),
    [("/api/screenshot", "screenshot"), ("/api/save", "save")],
)
def test_capture_endpoints_save_through_service(endpoint, prefix, tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    service.state.latest_frame = b"jpeg"

    with TestClient(app) as client:
        response = client.post(endpoint)

    assert response.status_code == 200
    body = response.json()
    assert Path(body["image_path"]).name.startswith(f"{prefix}-")
    assert Path(body["image_path"]).read_bytes() == b"jpeg"
    assert Path(body["metadata_path"]).is_file()


def test_latest_frame_returns_not_found_without_a_frame(tmp_path):
    app = make_app(tmp_path)

    with TestClient(app) as client:
        response = client.get("/frame/latest.jpg")

    assert response.status_code == 404


def test_latest_frame_returns_jpeg_from_service_state(tmp_path):
    app = make_app(tmp_path)
    app.state.dashboard_service.state.latest_frame = b"latest-jpeg"

    with TestClient(app) as client:
        response = client.get("/frame/latest.jpg")

    assert response.status_code == 200
    assert response.headers["content-type"] == "image/jpeg"
    assert response.content == b"latest-jpeg"


def test_latest_frame_prefers_requested_validated_sequence(tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    service.state.latest_frame = b"latest-jpeg"
    service._preview_frames[4] = b"requested-jpeg"

    with TestClient(app) as client:
        response = client.get("/frame/latest.jpg?f=4")

    assert response.status_code == 200
    assert response.content == b"requested-jpeg"


def test_browser_diagnostic_endpoint_records_frame_result(tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    service.log_browser_event = Mock()

    with TestClient(app) as client:
        response = client.post(
            "/api/diagnostics/browser",
            json={
                "event": "load",
                "frame_sequence": 12,
                "width": 1280,
                "height": 720,
                "risk": "medium",
                "frame_url": "http://127.0.0.1:8000/frame/latest.jpg?f=12",
            },
        )

    assert response.status_code == 204
    service.log_browser_event.assert_called_once_with(
        "load",
        12,
        1280,
        720,
        "medium",
        "http://127.0.0.1:8000/frame/latest.jpg?f=12",
    )


def test_state_websocket_broadcasts_and_always_removes_browser(tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    remove_browser = Mock(wraps=service.remove_browser)
    service.remove_browser = remove_browser

    with TestClient(app) as client:
        with client.websocket_connect("/ws/state") as websocket:
            message = websocket.receive_json()

    assert message["type"] == "state"
    assert message["state"]["running"] is False
    remove_browser.assert_called_once()
    assert not service._browser_clients


def test_state_websocket_closes_and_removes_when_registration_fails(tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    service.add_browser = Mock(return_value=False)
    service.remove_browser = Mock()
    service.broadcast = AsyncMock()
    service.stop = AsyncMock()

    with TestClient(app) as client:
        with client.websocket_connect("/ws/state") as websocket:
            message = websocket.receive()

    assert message["type"] == "websocket.close"
    service.broadcast.assert_not_awaited()
    service.remove_browser.assert_called_once()
    service.stop.assert_awaited_once_with()


@pytest.mark.parametrize("running", [False, True])
def test_app_shutdown_always_stops_service(running, tmp_path):
    app = make_app(tmp_path)
    service = app.state.dashboard_service
    service.state.running = running

    async def mark_stopped():
        service.state.running = False

    service.stop = AsyncMock(side_effect=mark_stopped)

    with TestClient(app):
        pass

    service.stop.assert_awaited_once_with()
    assert service.state.running is False


def test_dashboard_module_runs_uvicorn_with_dashboard_app(monkeypatch):
    captured = {}
    launched = []

    def fake_config(app, host, port, log_level):
        captured["app"] = app
        captured["host"] = host
        captured["port"] = port
        captured["log_level"] = log_level
        return object()

    class FakeServer:
        def __init__(self, config):
            self.config = config
            self.started = False
            captured["server"] = self

        def run(self):
            captured["ran"] = True

    monkeypatch.setattr("python_client.dashboard.uvicorn.Config", fake_config)
    monkeypatch.setattr("python_client.dashboard.uvicorn.Server", FakeServer)
    monkeypatch.setattr("python_client.dashboard.configure_stream_diagnostics", lambda path: Path(path))
    monkeypatch.setattr("python_client.dashboard.uvicorn.run", Mock())
    monkeypatch.setattr(
        "python_client.dashboard.start_browser_launcher",
        lambda url, ready: launched.append((url, ready)),
    )

    from python_client import dashboard

    dashboard.main(["--esp32-host", "192.168.1.3", "--port", "8001"])

    assert captured["host"] == "127.0.0.1"
    assert captured["port"] == 8001
    assert captured["log_level"] == "info"
    assert captured["app"].state.dashboard_service.host == "192.168.1.3"
    assert captured["ran"] is True
    assert launched[0][0] == "http://127.0.0.1:8001/"
    assert launched[0][1]() is False
    captured["server"].started = True
    assert launched[0][1]() is True


def test_dashboard_no_browser_still_runs_uvicorn(monkeypatch):
    from python_client import dashboard

    launcher = Mock()
    server = Mock()
    server.started = False
    monkeypatch.setattr(dashboard, "start_browser_launcher", launcher)
    monkeypatch.setattr(dashboard, "configure_stream_diagnostics", lambda path: Path(path))
    monkeypatch.setattr(dashboard.uvicorn, "Config", Mock(return_value=object()))
    monkeypatch.setattr(dashboard.uvicorn, "Server", Mock(return_value=server))
    monkeypatch.setattr(dashboard.uvicorn, "run", Mock())

    dashboard.main(
        ["--esp32-host", "192.168.1.3", "--device", "cpu", "--no-browser"]
    )

    launcher.assert_not_called()
    server.run.assert_called_once_with()


def test_stream_diagnostics_are_written_to_rotating_log(tmp_path):
    from python_client.dashboard import (
        DIAGNOSTIC_LOGGER_NAME,
        configure_stream_diagnostics,
    )

    log_path = configure_stream_diagnostics(tmp_path / "stream-diagnostics.log")
    logger = logging.getLogger(DIAGNOSTIC_LOGGER_NAME)
    try:
        logger.info("RX frame=7 sha256=abcdef0123456789")
        for handler in logger.handlers:
            handler.flush()
        assert "RX frame=7 sha256=abcdef0123456789" in log_path.read_text(
            encoding="utf-8"
        )
    finally:
        for handler in tuple(logger.handlers):
            logger.removeHandler(handler)
            handler.close()


@pytest.mark.parametrize(
    ("host", "expected"),
    [
        ("0.0.0.0", "http://127.0.0.1:8000/"),
        ("::", "http://[::1]:8000/"),
    ],
)
def test_dashboard_url_uses_matching_loopback_for_wildcard_hosts(host, expected):
    from python_client.dashboard import build_dashboard_url

    assert build_dashboard_url(host, 8000) == expected


def test_dashboard_url_preserves_regular_host():
    from python_client.dashboard import build_dashboard_url

    assert build_dashboard_url("localhost", 8001) == "http://localhost:8001/"


def test_browser_waits_for_dashboard_before_opening(monkeypatch):
    from python_client import dashboard

    states = iter([False, True])
    opened = []

    monkeypatch.setattr(dashboard.time, "sleep", lambda delay: None)
    dashboard._wait_and_open_browser(
        "http://127.0.0.1:8000/",
        ready=lambda: next(states),
        opener=lambda url: opened.append(url) or True,
        attempts=2,
    )

    assert opened == ["http://127.0.0.1:8000/"]


def test_browser_launch_failure_does_not_escape_worker():
    from python_client import dashboard

    def failed_opener(url):
        raise OSError("browser unavailable")

    dashboard._wait_and_open_browser(
        "http://127.0.0.1:8000/",
        ready=lambda: True,
        opener=failed_opener,
        attempts=1,
    )


def test_browser_thread_start_failure_does_not_raise(monkeypatch):
    from python_client import dashboard

    class BrokenThread:
        def __init__(self, **kwargs):
            raise RuntimeError("thread unavailable")

    monkeypatch.setattr(dashboard.threading, "Thread", BrokenThread)

    dashboard.start_browser_launcher(
        "http://127.0.0.1:8000/", ready=lambda: False
    )
