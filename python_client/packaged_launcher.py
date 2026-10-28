"""Windows executable entry point for the packaged dashboard."""

from __future__ import annotations

import logging
import os
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import TextIO


DEFAULT_ESP32_HOST = "192.168.1.3"
LOGGER = logging.getLogger(__name__)


class _TeeStream:
    """Mirror application output to the visible console and the log file."""

    def __init__(self, *streams: TextIO) -> None:
        self._streams = streams

    def write(self, data: str) -> int:
        for stream in self._streams:
            stream.write(data)
        return len(data)

    def flush(self) -> None:
        for stream in self._streams:
            stream.flush()

    def fileno(self) -> int:
        return self._streams[0].fileno()

    def isatty(self) -> bool:
        return self._streams[0].isatty()


def bundle_root() -> Path:
    frozen_root = getattr(sys, "_MEIPASS", None)
    if frozen_root is not None:
        return Path(frozen_root)
    return Path(__file__).resolve().parents[1]


def application_root() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[1]


def build_dashboard_args(argv: Sequence[str] | None = None) -> list[str]:
    forwarded = list(sys.argv[1:] if argv is None else argv)
    return [
        "--esp32-host",
        DEFAULT_ESP32_HOST,
        "--device",
        "cpu",
        "--model",
        str(bundle_root() / "yolo11n.pt"),
        "--capture-dir",
        str(application_root() / "captures"),
        "--diagnostic-log",
        str(application_root() / "logs" / "stream-diagnostics.log"),
        *forwarded,
    ]


def initialize_logging(
    root: Path | None = None,
    *,
    redirect_native: bool = True,
    console: bool = False,
) -> TextIO:
    log_dir = (root or application_root()) / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    stream = (log_dir / "Lab-Environment-Monitoring.log").open(
        "a", encoding="utf-8", buffering=1
    )
    if console:
        console_stdout = sys.__stdout__
        console_stderr = sys.__stderr__
        sys.stdout = _TeeStream(console_stdout, stream)
        sys.stderr = _TeeStream(console_stderr, stream)
    else:
        sys.stdout = stream
        sys.stderr = stream

    handlers = [logging.StreamHandler(stream)]
    if console:
        handlers.append(logging.StreamHandler(console_stdout))
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=handlers,
        force=True,
    )
    if redirect_native and not console:
        try:
            os.dup2(stream.fileno(), 1)
            os.dup2(stream.fileno(), 2)
        except OSError as exc:
            LOGGER.warning("Could not redirect native output: %s", exc)
    return stream


def launch(argv: Sequence[str] | None = None, *, runner=None) -> int:
    if runner is None:
        from python_client.dashboard import main as runner

    try:
        runner(build_dashboard_args(argv))
    except BaseException:
        LOGGER.exception("Lab-Environment-Monitoring stopped during startup")
        return 1
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    initialize_logging(console=True, redirect_native=False)
    LOGGER.info("Starting packaged Lab-Environment-Monitoring dashboard")
    return launch(argv)


if __name__ == "__main__":
    raise SystemExit(main())
