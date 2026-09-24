import socket
import subprocess
from unittest.mock import Mock

import pytest

from tools import smoke_test_windows_exe as smoke


def test_stop_process_terminates_windows_process_tree():
    process = Mock(pid=1234)
    process.poll.return_value = None
    run_taskkill = Mock()
    run_taskkill.return_value.returncode = 0

    smoke.stop_process(process, platform="win32", run_taskkill=run_taskkill)

    run_taskkill.assert_called_once_with(
        ["taskkill", "/PID", "1234", "/T", "/F"],
        check=False,
        stdout=smoke.subprocess.DEVNULL,
        stderr=smoke.subprocess.DEVNULL,
    )


def test_stop_process_reports_failed_windows_tree_cleanup():
    process = Mock(pid=1234)
    process.poll.return_value = None
    process.wait.side_effect = [subprocess.TimeoutExpired("video-ai", 5), None]
    run_taskkill = Mock()
    run_taskkill.return_value.returncode = 1

    with pytest.raises(RuntimeError, match="process tree"):
        smoke.stop_process(process, platform="win32", run_taskkill=run_taskkill)

    process.kill.assert_called_once_with()


def test_ensure_port_available_rejects_an_existing_listener():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        port = listener.getsockname()[1]

        with pytest.raises(RuntimeError, match=str(port)):
            smoke.ensure_port_available("127.0.0.1", port)
