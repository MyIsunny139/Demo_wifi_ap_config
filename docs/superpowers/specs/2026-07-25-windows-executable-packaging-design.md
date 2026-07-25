# Windows Executable Packaging Design

## Goal

Deliver the Python dashboard as a portable Windows x64 folder. The customer launches the complete application by double-clicking `video-ai.exe` and does not need Python installed.

## Distribution Format

- Build with PyInstaller in one-directory mode.
- Output to `dist/video-ai/`.
- Use windowed mode so no command prompt is shown.
- Bundle the dashboard static assets and `yolo11n.pt`.
- Keep PyTorch, Ultralytics, OpenCV, FastAPI, Uvicorn, and their runtime dependencies inside the distribution directory.
- Do not bundle development-only files such as tests, build caches, captures, or the source virtual environment.

One-directory mode is preferred over one-file mode because the machine-learning runtime is large. It avoids per-launch extraction, reduces startup time, and is less likely to trigger antivirus heuristics while still providing a single executable for the customer to double-click.

## Runtime Behavior

The packaged launcher supplies delivery defaults:

- ESP32 host: `192.168.1.3`
- ESP32 WebSocket port: `81`
- Inference device: `cpu`
- Model: bundled `yolo11n.pt`
- Dashboard bind address: `127.0.0.1`
- Dashboard port: `8000`

The existing dashboard opens the default browser after Uvicorn successfully binds. Additional command-line arguments remain available for support and diagnostics, including `--no-browser`.

## Runtime Paths

Read-only bundled resources are resolved from the PyInstaller bundle root. Writable files are always placed beside the executable:

- Logs: `logs/video-ai.log`
- Captures: `captures/`

This separation prevents writes into PyInstaller's internal resource directory and keeps customer data visible and portable.

## Logging And Errors

Because the executable has no console, the launcher creates `logs/` before importing the application and redirects standard output and standard error to `logs/video-ai.log`. Python logging and uncaught exceptions are also written there.

If browser launch, ESP32 connection, or inference fails, the local dashboard process remains available when possible and the failure is recorded in the log. Startup failures that prevent the server from running are recorded before the process exits.

## Build Structure

- `python_client/packaged_launcher.py`: packaged entry point and runtime path/log setup.
- `video-ai.spec`: reproducible PyInstaller configuration, bundled resources, hidden imports, and executable metadata.
- `tools/build_windows_exe.ps1`: installs/checks the build dependency, removes only packaging outputs, invokes PyInstaller, and reports the output path.
- `python_client/requirements-build.txt`: build-only PyInstaller dependency.

The existing source-mode command remains unchanged.

## Verification

Automated tests cover bundle/resource path selection, delivery defaults, writable paths, argument forwarding, and log initialization. The normal Python test suite must continue to pass.

After building, verification will:

1. Confirm `dist/video-ai/video-ai.exe`, static assets, and model resources exist.
2. Start the executable with `--no-browser` on a non-default test port.
3. Request `/`, `/static/dashboard.css`, and `/api/state`.
4. Start the dashboard processing flow to force model loading and check that failures are logged rather than hidden.
5. Stop the executable and confirm `logs/video-ai.log` was created.

The ESP32 firmware is outside this packaging change and will not be rebuilt.
