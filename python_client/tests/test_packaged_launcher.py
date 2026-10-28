import logging
import sys
from pathlib import Path
from unittest.mock import Mock

from python_client import packaged_launcher as launcher


def test_bundle_root_uses_pyinstaller_meipass(monkeypatch, tmp_path):
    bundle = tmp_path / "bundle"
    monkeypatch.setattr(sys, "_MEIPASS", str(bundle), raising=False)

    assert launcher.bundle_root() == bundle


def test_application_root_uses_executable_directory_when_frozen(
    monkeypatch, tmp_path
):
    executable = tmp_path / "Lab-Environment-Monitoring.exe"
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setattr(sys, "executable", str(executable))

    assert launcher.application_root() == tmp_path


def test_build_dashboard_args_uses_delivery_defaults(monkeypatch, tmp_path):
    monkeypatch.setattr(launcher, "bundle_root", lambda: tmp_path / "bundle")
    monkeypatch.setattr(launcher, "application_root", lambda: tmp_path / "app")

    args = launcher.build_dashboard_args(["--no-browser", "--port", "8765"])

    assert args[:4] == ["--esp32-host", "192.168.1.3", "--device", "cpu"]
    assert args[4:6] == ["--model", str(tmp_path / "bundle" / "yolo11n.pt")]
    assert args[6:8] == ["--capture-dir", str(tmp_path / "app" / "captures")]
    assert args[8:10] == [
        "--diagnostic-log",
        str(tmp_path / "app" / "logs" / "stream-diagnostics.log"),
    ]
    assert args[-3:] == ["--no-browser", "--port", "8765"]


def test_initialize_logging_redirects_python_and_native_streams(monkeypatch, tmp_path):
    original_stdout = sys.stdout
    original_stderr = sys.stderr
    duplicate_calls = []
    configure_logging = Mock()
    monkeypatch.setattr(launcher.os, "dup2", lambda source, target: duplicate_calls.append((source, target)))
    monkeypatch.setattr(launcher.logging, "basicConfig", configure_logging)

    stream = launcher.initialize_logging(tmp_path)
    stream_fd = stream.fileno()
    try:
        print("packaged stdout")
        print("packaged stderr", file=sys.stderr)
        stream.flush()
    finally:
        sys.stdout = original_stdout
        sys.stderr = original_stderr
        stream.close()

    log_path = tmp_path / "logs" / "Lab-Environment-Monitoring.log"
    assert "packaged stdout" in log_path.read_text(encoding="utf-8")
    assert "packaged stderr" in log_path.read_text(encoding="utf-8")
    assert duplicate_calls == [(stream_fd, 1), (stream_fd, 2)]
    configure_logging.assert_called_once()


def test_launch_forwards_packaged_arguments(monkeypatch):
    runner = Mock()
    monkeypatch.setattr(launcher, "build_dashboard_args", lambda argv: ["packed", *argv])

    result = launcher.launch(["--no-browser"], runner=runner)

    assert result == 0
    runner.assert_called_once_with(["packed", "--no-browser"])


def test_launch_records_startup_failure(monkeypatch):
    runner = Mock(side_effect=RuntimeError("startup failed"))
    logger = Mock()
    monkeypatch.setattr(launcher, "LOGGER", logger)

    result = launcher.launch([], runner=runner)

    assert result == 1
    logger.exception.assert_called_once_with("Lab-Environment-Monitoring stopped during startup")
