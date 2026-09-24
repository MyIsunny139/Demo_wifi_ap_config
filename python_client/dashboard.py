"""Start the browser dashboard and local Python service."""

from __future__ import annotations

import argparse
import logging
import threading
import time
import webbrowser
from collections.abc import Callable
from logging.handlers import RotatingFileHandler
from pathlib import Path

import uvicorn

from .dashboard_app import create_app


LOGGER = logging.getLogger(__name__)
DIAGNOSTIC_LOGGER_NAME = "video_ai.stream_diagnostics"


def configure_stream_diagnostics(path: Path | str) -> Path:
    log_path = Path(path).expanduser().resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    logger = logging.getLogger(DIAGNOSTIC_LOGGER_NAME)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    for handler in tuple(logger.handlers):
        logger.removeHandler(handler)
        handler.close()
    handler = RotatingFileHandler(
        log_path,
        maxBytes=10 * 1024 * 1024,
        backupCount=3,
        encoding="utf-8",
    )
    handler.setFormatter(
        logging.Formatter("%(asctime)s.%(msecs)03d %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    )
    logger.addHandler(handler)
    return log_path


def build_dashboard_url(host: str, port: int) -> str:
    if host == "0.0.0.0":
        browser_host = "127.0.0.1"
    elif host == "::":
        browser_host = "::1"
    else:
        browser_host = host
    if ":" in browser_host and not browser_host.startswith("["):
        browser_host = f"[{browser_host}]"
    return f"http://{browser_host}:{port}/"


def _wait_and_open_browser(
    url: str,
    *,
    ready: Callable[[], bool],
    opener: Callable[[str], bool] = webbrowser.open,
    attempts: int = 100,
    interval: float = 0.1,
) -> None:
    for attempt in range(attempts):
        try:
            is_ready = ready()
        except Exception as exc:
            LOGGER.warning("Could not check dashboard readiness: %s", exc)
            return

        if is_ready:
            try:
                if not opener(url):
                    LOGGER.warning("No browser accepted the dashboard URL: %s", url)
            except Exception as exc:
                LOGGER.warning("Could not open dashboard browser: %s", exc)
            return

        if attempt + 1 < attempts:
            time.sleep(interval)

    LOGGER.warning("Dashboard did not become ready for browser launch: %s", url)


def start_browser_launcher(url: str, ready: Callable[[], bool]) -> None:
    try:
        threading.Thread(
            target=_wait_and_open_browser,
            args=(url,),
            kwargs={"ready": ready},
            name="dashboard-browser-launcher",
            daemon=True,
        ).start()
    except Exception as exc:
        LOGGER.warning("Could not start dashboard browser launcher: %s", exc)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="Local web host for the browser dashboard",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=8000,
        help="Local web port for the browser dashboard",
    )
    parser.add_argument(
        "--esp32-host",
        required=True,
        help="ESP32 IP address or host name",
    )
    parser.add_argument(
        "--esp32-port",
        type=int,
        default=81,
        help="ESP32 WebSocket port",
    )
    parser.add_argument(
        "--model", default="smoking.onnx", help="Smoking-trained YOLO model path"
    )
    parser.add_argument(
        "--confidence",
        type=float,
        default=0.4,
        help="YOLO confidence threshold",
    )
    parser.add_argument(
        "--device",
        default=None,
        help="Ultralytics device, for example 0 or cpu",
    )
    parser.add_argument(
        "--capture-dir",
        default="captures",
        help="Directory used to save screenshots",
    )
    parser.add_argument(
        "--diagnostic-log",
        default="logs/stream-diagnostics.log",
        help="Detailed rotating stream diagnostic log",
    )
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="Do not open the dashboard in the default browser",
    )
    return parser


def main(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    if not 0.0 < args.confidence <= 1.0:
        raise SystemExit("--confidence must be in (0, 1]")
    if not 1 <= args.port <= 65535:
        raise SystemExit("--port must be in [1, 65535]")
    if not 1 <= args.esp32_port <= 65535:
        raise SystemExit("--esp32-port must be in [1, 65535]")

    diagnostic_path = configure_stream_diagnostics(args.diagnostic_log)
    logging.getLogger(DIAGNOSTIC_LOGGER_NAME).info(
        "SESSION start esp32=%s:%d model=%s device=%s diagnostic_log=%s",
        args.esp32_host,
        args.esp32_port,
        args.model,
        args.device or "auto",
        diagnostic_path,
    )

    app = create_app(
        host=args.esp32_host,
        port=args.esp32_port,
        model=args.model,
        confidence=args.confidence,
        device=args.device,
        capture_dir=args.capture_dir,
    )
    config = uvicorn.Config(
        app,
        host=args.host,
        port=args.port,
        log_level="info",
    )
    server = uvicorn.Server(config)
    if not args.no_browser:
        start_browser_launcher(
            build_dashboard_url(args.host, args.port),
            ready=lambda: server.started,
        )
    server.run()


if __name__ == "__main__":
    main()
