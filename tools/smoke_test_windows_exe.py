"""Smoke-test the packaged Windows dashboard executable."""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


def request(url: str, *, method: str = "GET") -> bytes:
    payload = b"" if method == "POST" else None
    message = urllib.request.Request(url, data=payload, method=method)
    with urllib.request.urlopen(message, timeout=2) as response:
        if response.status != 200:
            raise RuntimeError(f"{url} returned HTTP {response.status}")
        return response.read()


def read_state(base_url: str) -> dict[str, Any]:
    return json.loads(request(f"{base_url}/api/state"))


def wait_for_server(process: subprocess.Popen[bytes], base_url: str) -> None:
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Lab-Environment-Monitoring.exe exited with code {process.returncode}")
        try:
            read_state(base_url)
            return
        except (OSError, urllib.error.URLError, json.JSONDecodeError):
            time.sleep(0.2)
    raise TimeoutError("Timed out waiting for the packaged dashboard")


def wait_for_model(base_url: str) -> dict[str, Any]:
    request(f"{base_url}/api/start", method="POST")
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        state = read_state(base_url)
        if state["processing"] != "starting":
            if not state["running"] and state["last_error"]:
                raise RuntimeError(f"Model startup failed: {state['last_error']}")
            return state
        time.sleep(0.25)
    raise TimeoutError("Timed out waiting for the YOLO model")


def ensure_port_available(host: str, port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        if sys.platform == "win32":
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        try:
            probe.bind((host, port))
        except OSError as exc:
            raise RuntimeError(f"Smoke-test port {host}:{port} is unavailable") from exc


def stop_process(
    process: subprocess.Popen[bytes],
    *,
    platform: str = sys.platform,
    run_taskkill=subprocess.run,
) -> None:
    if process.poll() is not None:
        return
    if platform == "win32":
        result = run_taskkill(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        if result.returncode != 0:
            raise RuntimeError(
                f"Failed to terminate Lab-Environment-Monitoring process tree (PID {process.pid})"
            )
        return
    process.terminate()
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--port", type=int, default=8765)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        raise FileNotFoundError(executable)

    app_dir = executable.parent
    base_url = f"http://127.0.0.1:{args.port}"
    ensure_port_available("127.0.0.1", args.port)
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    process = subprocess.Popen(
        [str(executable), "--no-browser", "--port", str(args.port)],
        cwd=app_dir,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=creation_flags,
    )
    try:
        wait_for_server(process, base_url)
        if b"Lab-Environment-Monitoring" not in request(f"{base_url}/"):
            raise RuntimeError("Dashboard HTML is missing expected content")
        if not request(f"{base_url}/static/dashboard.css"):
            raise RuntimeError("Dashboard stylesheet is empty")
        state = wait_for_model(base_url)
        print(f"Dashboard state after model load: {state['processing']}")
    finally:
        stop_process(process)

    log_path = app_dir / "logs" / "Lab-Environment-Monitoring.log"
    if not log_path.is_file() or log_path.stat().st_size == 0:
        raise RuntimeError("Packaged log file was not created")
    print(f"Smoke test passed: {executable}")


if __name__ == "__main__":
    main()
